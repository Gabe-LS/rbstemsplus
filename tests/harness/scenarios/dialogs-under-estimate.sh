# scenario: what updates under each kind of dialog today (mac-core §2.3, §3.7; PLAN I2): the
# Prepare tab's estimate streams its rows while the dialog is up, one kind at a time: a main-thread
# confirmation (Install Demucs v4?), a work-queue alert ("rekordbox open" during an install), the
# password sheet (Uninstall Stems Cache) and "Permission needed" (Reinstall Stems Cache, App
# Management refusing). trace.py dialogs lists, per dialog, whether the rows changed under it: the
# record G2.1's waived difference is checked against (1.4.0 updates under all four)
# commit: dialog:main-thread, dialog:work-queue, dialog:password, dialog:permission, I2
# tag: Prepare
# compare: properties
# server: dist-test
# allow: c.status.str c.spinner.anim c.cacheIcon. c.cacheLabel.tip status.cacheTip
# mask: Downloading the Demucs v4 model… \d+%
# under-dialog: prepare.tracksTable
# must-show: c.spinner.anim=1>0
# must-show: c.cacheIcon.img=off>on
# must-show: c.status.str=Installing Stems Cache (this takes about a minute)…
# must-show[.p2]: c.spinner.anim=1>0>1>0>1>0
# must-show[.p2]: c.status.str=Uninstalling Stems Cache…>Reinstalling Stems Cache (this takes about a minute)…
# (under-dialog: what the rows did under each dialog is compared dialog by dialog; compare --i2
# requires 1.4.0 to update them under the three dialogs A froze them under, PLAN I2)
# (Stems Cache is installed for real first: the download's progress lines, and its light with
# rekordbox's own model waiting for the model's checksum, are compared as a subsequence of A's;
# the rows and every other field exactly: each line waits for its gate)
setup() { fact_welcomed; fact_engine 0002; fixture_library; fact_sudo_nopasswd; fact_app_management allow; }
under() { helper_go "${1}1"; settle 300; helper_go "${1}2"; settle 300; }
rest() { helper_go "${1}3"; helper_go "${1}4"; wait_for 10000 "prepare.prepareBtn.en=1"; settle; }
steps() {
  launch
  settle
  # Stems Cache in rekordbox for real: the Prepare tab checks playlists only with it
  click c.installCacheBtn
  next_dialog 5000; answer Install
  wait_for 30000 "c.spinner.anim=1"
  wait_for 300000 "c.spinner.anim=0"
  settle 1000
  # App Management refuses this build again from here on: an Allow holds for the process that got
  # it, so the app is opened again (its second trace) with the refusal in place
  quit_app
  fact_app_management deny
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  estimate_friday a
  select_playlist 2
  helper_calls estimate-2 1
  # 1. a main-thread dialog: the confirmation, asked from the menu's action
  menu "menu.Manage.Demucs v4.Install"
  next_dialog 5000; settle
  under a
  answer Cancel; settle
  rest a
  # 2. a work-queue alert: rekordbox opened before the install checks it (after the download)
  check_gated b
  menu "menu.Manage.Demucs v4.Install"
  next_dialog 5000; settle
  fact_rekordbox_open                    # after the menu's own check (preflight), before the install's
  answer Install
  next_dialog 300000; settle             # "rekordbox open" (Try Again / Cancel), from the work queue
  under b
  answer Cancel
  wait_for 30000 "c.spinner.anim=0"
  fact_rekordbox_closed
  settle
  tab Prepare; settle
  rest b
  # 3. the password sheet (from the work queue)
  fact_sudo_password
  check_gated c
  menu "menu.Manage.Stems Cache.Uninstall…"
  next_dialog 5000; settle
  answer Uninstall
  next_dialog 30000; settle              # Password needed
  under c
  answer Cancel
  wait_for 30000 "c.spinner.anim=0"
  settle
  tab Prepare; settle
  rest c
  # 4. "Permission needed" (from the work queue): App Management refuses this build now
  fact_sudo_nopasswd
  fact_app_management deny
  check_gated d
  menu "menu.Manage.Stems Cache.Reinstall"
  next_dialog 5000; settle
  answer Reinstall
  next_dialog 300000; settle             # Permission needed
  under d
  answer Cancel
  wait_for 30000 "c.spinner.anim=0"
  settle
  tab Prepare; settle
  rest d
}
