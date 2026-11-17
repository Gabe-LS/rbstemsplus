# scenario: P9: an estimate's rows reaching the window while a dialog is up, two rounds a launch
# (PERF_ROUNDS) of each kind: none (the baseline), a main-thread confirmation, a work-queue alert
# ("rekordbox open" during an install), the password sheet and "Permission needed" (mac-core
# §2.1, §3.7). Each helper line let go under the dialog is one input batch; P9 is from its mark to
# the turn that drew its rows (tests/perf/stats.py), per kind P9.<kind>
# tag: first
# mode: perf
# metrics: P9
# server: make plain
# Stems Cache is installed for real first (about a minute, not measured): the Prepare tab checks
# playlists only with it.
setup() { fact_welcomed; fact_engine 0002; fixture_library; fact_sudo_nopasswd; fact_app_management allow; }
# the whole estimate released while the dialog is up, then a moment for the rows
# each helper line let go is one input batch, marked right before (tests/perf/stats.py's P9), then
# a moment for its row; the batch must draw while the dialog is up (B), A freezes under three kinds
under() { local k; for k in 1 2 3 4; do mark "P9 batch"; helper_go "$1$k"; CMD_WAIT_S=10 cmd quiet 300 5000; done; }
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  click c.installCacheBtn
  next_dialog 5000; answer Install
  wait_for 30000 "c.spinner.anim=1"
  wait_for 300000 "c.spinner.anim=0"
  CMD_WAIT_S=20 cmd quiet 1500 15000
  # App Management refuses this build again from here on (an Allow holds for the process that got
  # it): the app opened again with the refusal in place
  quit_app
  fact_app_management deny
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  estimate_friday z
  select_playlist 2
  helper_calls estimate-2 1
  helper_go z1; helper_go z2; helper_go z3; helper_go z4     # the first check: not a P9 round
  wait_for 10000 "prepare.prepareBtn.en=1"
  local i
  for i in $(seq 1 "${PERF_ROUNDS:-2}"); do
    mark "P9 none"
    check_gated "n$i"; under "n$i"
    wait_for 10000 "prepare.prepareBtn.en=1"
    mark "P9 main"
    check_gated "m$i"
    menu "menu.Manage.Demucs v4.Install"
    next_dialog 5000; under "m$i"; answer Cancel
    wait_for 10000 "prepare.prepareBtn.en=1"
    mark "P9 workqueue"
    check_gated "w$i"
    menu "menu.Manage.Demucs v4.Install"
    next_dialog 5000
    fact_rekordbox_open                   # after the menu's own check, before the install's
    answer Install
    next_dialog 300000; under "w$i"; answer Cancel
    wait_for 30000 "c.spinner.anim=0"; fact_rekordbox_closed
    tab Prepare; wait_for 10000 "prepare.prepareBtn.en=1"
    mark "P9 password"
    fact_sudo_password
    check_gated "p$i"
    menu "menu.Manage.Stems Cache.Uninstall…"
    next_dialog 5000; answer Uninstall
    next_dialog 30000; under "p$i"; answer Cancel
    wait_for 30000 "c.spinner.anim=0"
    tab Prepare; wait_for 10000 "prepare.prepareBtn.en=1"
    mark "P9 permission"
    fact_sudo_nopasswd; fact_app_management deny
    check_gated "q$i"
    menu "menu.Manage.Stems Cache.Reinstall"
    next_dialog 5000; answer Reinstall
    next_dialog 300000; under "q$i"; answer Cancel
    wait_for 30000 "c.spinner.anim=0"
    tab Prepare; wait_for 10000 "prepare.prepareBtn.en=1"
  done
  mark "P9 end"
  flush
}
