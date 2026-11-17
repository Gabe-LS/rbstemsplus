# scenario: Uninstall RB Stems Plus… when rekordbox's own model can't be put back (rekordbox has a
# model this app doesn't know, with Pioneer's saved copy here): "Uninstall stopped" offers Remove RB
# Stems Plus Anyway, which deletes only this account's own files and keeps the saved copy of
# rekordbox's model (the last alert says where); the app deletes itself. Destructive: the app is
# installed again before the next scenario (run.sh)
# commit: 935110a
# tag: first
# compare: exact
setup() { fact_welcomed; fact_plus_on; fact_engine 0002; fact_chosen 1 0; fact_original_saved; }
steps() {
  launch
  settle
  menu "menu.App.Uninstall RB Stems Plus…"
  next_dialog 10000; settle              # Uninstall RB Stems Plus?
  answer Uninstall
  next_dialog 60000; settle              # Uninstall stopped: Remove RB Stems Plus Anyway
  answer "Remove RB Stems Plus Anyway"
  next_dialog 30000; settle              # RB Stems Plus is removed
  answer Done
  local i
  for i in $(seq 1 300); do [ -e "$APP" ] || break; sleep 0.1; done
  check '[ ! -e "$APP" ]' "the app deleted itself"
  check 'ls "$SUP/originals/"*.onnx >/dev/null 2>&1' "the saved copy of rekordbox's model stays"
  check '[ "$(ls "$SUP" | tr "\n" " ")" = "originals " ]' "only the originals folder is left"
}
