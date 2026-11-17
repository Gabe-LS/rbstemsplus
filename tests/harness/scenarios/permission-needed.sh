# scenario: "Permission needed": Install Stems Cache from the TEST build's local server, and App
# Management refuses the root script (this build has no grant yet): the app's own dialog with the
# steps (from the work queue); Try Again runs the root script again with the action's own
# status line again ("Trying again…" may show for a frame before it); refused again; Cancel stops
# it, nothing changed in rekordbox and no failure alert (the user knows)
# commit: 0d1117a, dialog:permission
# tag: first
# compare: properties
# server: dist-test
# allow: c.status.str c.spinner.anim
# must-show: c.status.str=Installing Stems Cache (this takes about a minute)…>Installing Stems Cache (this takes about a minute)…
# must-show: c.spinner.anim=1>0>1>0
# (the download's progress lines depend on the transfer; between the attempts the status line and
# the spinner are set from the work queue in separate main-queue blocks, so a run shows "Trying
# again…" for a frame or not: these two compared as a subsequence of A's, every other field exactly)
setup() { fact_welcomed; fact_sudo_nopasswd; fact_app_management deny; }
steps() {
  launch
  settle
  click c.installCacheBtn
  next_dialog 5000; settle
  answer Install                         # the confirmation
  next_dialog 120000; settle             # Permission needed
  answer "Try Again"
  next_dialog 60000; settle              # refused again
  answer Cancel
  wait_for 15000 "c.spinner.anim=0"
  settle
  check 'cmp -s "$RBLIB" "$H/pristine/libonnxruntime.1.18.0.dylib"' "rekordbox's library is Pioneer's: nothing changed"
}
