# scenario: Install Stems Cache from the TEST build's local server with App Management allowed for
# this build (no password: sudo remembers none is needed): the root script's status line ("this
# takes about a minute"), the light, the buttons; then Uninstall Stems Cache puts Pioneer's files
# back (the root script again)
# commit: 617d2d2, flow:install, flow:uninstall
# tag: first
# compare: properties
# server: dist-test
# allow: c.status.str c.cacheIcon. c.cacheLabel.tip status.cacheTip.str
# must-show: c.spinner.anim=1>0>1>0
# must-show: c.cacheIcon.img=off>on>off
# must-show: c.status.str=Installing Stems Cache (this takes about a minute)…>Uninstalling Stems Cache…
# (the download's progress lines depend on the transfer, and the light with rekordbox's own model
# waits for the model's checksum, worked out in the background: those fields by their final value
# and as a subsequence of A's; every other field exactly)
setup() { fact_welcomed; fact_engine 0002; fact_sudo_nopasswd; fact_app_management allow; }
steps() {
  launch
  settle
  click c.installCacheBtn
  next_dialog 5000; settle
  answer Install
  wait_for 300000 "c.spinner.anim=1"
  wait_for 300000 "c.spinner.anim=0"
  settle 1000
  check '! cmp -s "$RBLIB" "$H/pristine/libonnxruntime.1.18.0.dylib"' "the bridge is in rekordbox"
  check 'grep -q "cache=1" "$SUP/installed"' "Stems Cache chosen"
  click c.uninstallCacheBtn
  next_dialog 5000; settle
  answer Uninstall
  wait_for 30000 "c.spinner.anim=1"
  wait_for 300000 "c.spinner.anim=0"
  settle 1000
  check 'cmp -s "$RBLIB" "$H/pristine/libonnxruntime.1.18.0.dylib"' "Pioneer's library is back in rekordbox"
}
