# G1 item 1's regression coverage, from 1.3.0's code (not an oracle trace: run with vm.sh run,
# never recorded; supporting evidence only, G1 item 1 itself is the owner's round from the
# unmodified tag builds: tests/harness/g1-round.sh): mac-A (1.3.0's release code, the probe added:
# tests/perf/mac-a.sh checks the code is 1.3.0's) installs Stems Cache and Demucs v4 from the
# core-linked 1.4.0 TEST build's own server, then updates in-app to it. The new app is the core
# build (core_selftest in it, --swap ran it), no quarantine, and registers the watcher again: its
# registration record changes to the new helper's code, "watcher: installed" is logged after the
# update, and a WatchPaths trigger runs it. App Management is EMULATED (DJ's TCC row): setup's
# Allow row for mac-A's code is kept through the update and what the new build's first root run
# meets is recorded, "Permission needed" or success (the result's note); after a refusal the
# Allow is emulated for the new code and the app opened again (a decision holds for the process
# that met it). Reinstall and Install work; Uninstall RB Stems Plus… leaves rekordbox byte for
# byte as before.
#   tests/harness/vm.sh run g1 g1-update-core   (mac-A pushed; the core build in build/g1-core)
# tag: first
# compare: properties
# server: build/g1-core/dist-test
setup() {
  fact_welcomed; fact_engine 0002; fact_sudo_nopasswd; fact_app_management allow
  rb_tree > "$RUN/rekordbox-before.txt"
}
# rekordbox's file tree: every file's checksum, every link's target, every folder (with owner and mode)
rb_tree() {
  (cd "/Applications/rekordbox 7" && find . \( -type f -exec shasum -a 256 {} + \) -o \( -type l -exec sh -c 'for l; do echo "link $l -> $(readlink "$l")"; done' sh {} + \) -o \( -type d -exec stat -f 'dir %N %Su %Sp' {} + \) | LC_ALL=C sort)
  shasum -a 256 "$MODELDIR/hdemucs.onnx"
}
action() {                                # action BUTTON VERB: a Manage button, its confirmation, its end
  click "$1"; next_dialog 5000; answer "$2"
  wait_for 30000 "c.spinner.anim=1"; wait_for 600000 "c.spinner.anim=0"; settle 1000
}
agent_runs() { launchctl print "gui/$UID_/io.github.rbstemsplus.watcher" 2>/dev/null | sed -n 's/^[[:space:]]*runs = //p' | tr -d ' '; }
steps() {
  echo "note: App Management emulated (DJ's TCC rows), not the user's Allow: G1 item 1 needs the owner's round" >> "$RUN/result"
  launch
  wait_for 60000 "c.updateItem.hid=0"
  settle
  action c.installCacheBtn Install
  action c.installPlusBtn Install
  check 'launchctl print "gui/$UID_/io.github.rbstemsplus.watcher" >/dev/null 2>&1' "1.3.0's watcher registered"
  cp "$SUP/watcher.registered" "$RUN/registered-before.txt" 2>/dev/null || echo none > "$RUN/registered-before.txt"
  local n0; n0="$(wc -l < "$LOGS/app.log" | tr -d ' ')"
  menu c.updateItem
  next_dialog 5000; settle
  local before; before="$(traces | wc -l | tr -d ' ')"
  answer Update
  wait_new_trace "$before" 3000 || return 1
  wait_event 30000 windowDrawn 1
  settle 2000
  check '[ "$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)" = 1.4.0 ]' "the app is 1.4.0"
  check 'grep -aq core_selftest "$APP/Contents/MacOS/RB Stems Plus"' "it is the core-linked build (core_selftest in it; the VM has no nm)"
  check 'tail -n +$((n0 + 1)) "$LOGS/app.log" | grep -q "update to 1.4.0: updated"' "the update script says updated"
  check '! xattr -r "$APP" 2>/dev/null | grep -q com.apple.quarantine' "no quarantine on the new app"
  check '[ "$(count_of dialog)" = 0 ]' "no Stems Cache update offer (the same bridge)"
  # the watcher registered again for the new helper (the same path as 1.3.0's: the record decides)
  check '[ -s "$SUP/watcher.registered" ] && ! cmp -s "$SUP/watcher.registered" "$RUN/registered-before.txt"' "the watcher's registration record changed to the new helper's code"
  check 'tail -n +$((n0 + 1)) "$LOGS/app.log" | grep -q "watcher: installed"' "the new app registered the watcher (\"watcher: installed\" after the update)"
  local r0 r1 i; r0="$(agent_runs)"
  touch "$RB/Contents/Info.plist"            # a WatchPaths trigger; rekordbox's content unchanged
  for i in $(seq 1 30); do sleep 2; r1="$(agent_runs)"; [ -n "$r1" ] && [ "$r1" != "$r0" ] && break; done
  check '[ -n "$r1" ] && [ "$r1" != "$r0" ]' "a WatchPaths trigger ran the new watcher (launchd runs $r0 -> $r1)"
  # App Management for the new code: whatever the kept rows give, recorded
  local d; d="$(count_of dialog)"
  click c.reinstallCacheBtn; next_dialog 5000; answer Reinstall
  d=$((d + 1))
  local met=""
  for i in $(seq 1 600); do
    if QUIET_FAIL=1 wait_event 500 dialog $((d + 1)); then met=refused; break; fi
    if QUIET_FAIL=1 wait_for 100 "c.spinner.anim=0"; then met=allowed; break; fi
  done
  echo "note: the new build's first root run under the kept TCC rows: ${met:-neither (timeout)}" >> "$RUN/result"
  check '[ -n "$met" ]' "the new build's first root run: Permission needed or success"
  if [ "$met" = refused ]; then
    check 'grep -q "Permission needed" "$TRACE"' "the refusal is the app's Permission needed"
    answer Cancel
    wait_for 60000 "c.spinner.anim=0"; settle
    quit_app
    fact_app_management allow                # the emulated Allow for the new build's code
    launch
    settle
    action c.reinstallCacheBtn Reinstall
  else
    settle 1000
  fi
  action c.reinstallPlusBtn Reinstall
  action c.uninstallPlusBtn Uninstall
  action c.installPlusBtn Install
  check 'grep -q "cache=1" "$SUP/installed" && grep -q "model=1" "$SUP/installed"' "both installed with the new app"
  menu "menu.App.Uninstall RB Stems Plus…"
  next_dialog 10000; answer Uninstall
  next_dialog 600000; settle             # RB Stems Plus is uninstalled
  answer Done
  for i in $(seq 1 300); do [ -e "$APP" ] || break; sleep 0.1; done
  check '[ ! -e "$APP" ] && [ ! -e "$SUP" ] && [ ! -e "$LOGS" ] && [ ! -e "$ROOTDIR" ] && [ ! -e "$UH/Library/LaunchAgents/io.github.rbstemsplus.watcher.plist" ]' "nothing of RB Stems Plus is left"
  rb_tree > "$RUN/rekordbox-after.txt"
  check 'cmp -s "$RUN/rekordbox-before.txt" "$RUN/rekordbox-after.txt"' "rekordbox byte for byte as before (its tree and model)"
  check 'codesign --verify --deep --strict "$RB" 2>/dev/null' "rekordbox verifies"
}
