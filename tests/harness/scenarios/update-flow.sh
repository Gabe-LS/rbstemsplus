# scenario: the in-app update to a newer TEST build served locally (the build under test repacked
# with its patch number plus one: mac-A as 1.3.1, 1.4.0 as 1.4.1): the
# payload.json downloaded at launch shows the notice in the status line and "Update RB Stems
# Plus…" in the app menu; Update asks, shows "Updating RB Stems Plus…", the app quits, the update
# script replaces it and opens the new one (its own probe: the second trace), whose window shows
# no notice
# commit: 4d2e00e, 038304d, flow:update, 10e616d
# tag: first
# compare: exact
# server: make update --app-version next
# mask: (?<![\d.])\d+\.\d+\.\d+(?![\d.])
# (the versions in the notice and the dialog are each build's own plus one: masked, so A's and B's
# texts compare)
setup() { fact_welcomed; }
steps() {
  local cur next
  cur="$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)"
  next="${cur%.*}.$(( ${cur##*.} + 1 ))"
  launch
  wait_for 30000 "c.updateItem.hid=0"
  settle
  record_pids
  menu c.updateItem
  next_dialog 5000; settle               # Update RB Stems Plus to <next>?
  local before; before="$(traces | wc -l | tr -d ' ')"
  answer Update
  wait_new_trace "$before" 1800 || return 1
  wait_event 30000 windowDrawn 1
  record_pids
  settle
  check '[ "$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)" = "$next" ]' "the app is $next now"
  check '[ "$(pgrep -x "RB Stems Plus" | wc -l | tr -d " ")" = 1 ]' "one copy runs: the new one"
  check '[ ! -e "$SUP/update" ]' "the update folder is gone"
}
