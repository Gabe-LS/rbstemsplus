# scenario: Uninstall Stems Cache asks for the password in the app's own sheet (from the work
# queue), "Wrong password" after a wrong one, until cancelled; the window under the sheet; the
# menus while it is up. The password typed is a fixed wrong one, never a real password.
# commit: 00d930f, d043f9b, 0d1117a
# tag: first
# compare: exact
# allow: menu.App.Hide
# (Hide validates off for a frame in some runs, between the password sheet's end and the next
# sheet while sudo checks the password: compared by its final value)
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1"; fact_sudo_password; }
steps() {
  launch
  settle
  click c.uninstallCacheBtn
  next_dialog 5000; settle
  answer "Uninstall"                     # the confirmation
  :
  next_dialog 15000; settle              # Password needed
  cmd password "not-the-password"
  answer Continue
  :
  next_dialog 15000; settle              # Wrong password
  answer Cancel
  wait_for 15000 "c.spinner.anim=0"
  settle
}
