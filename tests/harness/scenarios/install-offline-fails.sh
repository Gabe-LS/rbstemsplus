# scenario: Install Demucs v4 chosen from the menu while the Status tab is in front, with no
# download server: the Manage tab comes to the front with the spinner and the status line, the
# work queue's "Download failed" alert (asked from the work queue), OK, everything as before
# commit: 0a63096, 91baa8a, start/install flow
# tag: first
# compare: exact
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1"; }
steps() {
  launch
  settle
  tab Status; settle
  menu "menu.Manage.Demucs v4.Install"
  next_dialog 5000; settle
  answer Install
  :
  next_dialog 30000                     # the failure, from the work queue
  settle
  answer OK
  wait_for 10000 "c.spinner.anim=0"
  settle
}
