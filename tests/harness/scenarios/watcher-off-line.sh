# scenario: the status line when the reinstall reminders are off: Install Stems Cache turns on the
# Stems Cache another account installed (no download but payload.json, no password) and installs
# the watcher's LaunchAgent; the user turns RB Stems Plus Watcher off (launchctl disable, as Login
# Items does): coming back to the app the status line says so; turned on again, the line goes
# commit: 22e8b33
# tag: first
# compare: exact
# server: dist-test
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 0; fact_engine 0002; }
steps() {
  launch
  settle
  click c.installCacheBtn
  next_dialog 5000; settle
  answer Install
  wait_for 30000 "c.spinner.anim=1"
  wait_for 60000 "c.spinner.anim=0"
  settle
  check 'as_user launchctl print "gui/$UID_/io.github.rbstemsplus.watcher" >/dev/null 2>&1' "the watcher is registered"
  finder_front
  as_user launchctl disable "gui/$UID_/io.github.rbstemsplus.watcher"
  VERB=reopen activate
  settle 1000
  finder_front
  as_user launchctl enable "gui/$UID_/io.github.rbstemsplus.watcher"
  VERB=reopen activate
  settle 1000
}
