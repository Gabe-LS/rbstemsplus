# scenario: Uninstall RB Stems Plus… stops when Stems Cache can't be removed, with the root
# script's own reason (here: no saved copy of Pioneer's files, as a bridge put in by hand leaves
# it): "Uninstall stopped", nothing else deleted, the app stays
# commit: b763370
# tag: first
# compare: exact
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1"; fact_engine 0002; fact_sudo_nopasswd; }
steps() {
  launch
  settle
  menu "menu.App.Uninstall RB Stems Plus…"
  next_dialog 10000; settle              # Uninstall RB Stems Plus?
  answer Uninstall
  next_dialog 60000; settle              # Uninstall stopped, with the reason
  answer OK
  wait_for 30000 "c.spinner.anim=0"
  settle
  check '[ -e "$APP" ] && [ -d "$SUP" ]' "nothing else was deleted"
}
