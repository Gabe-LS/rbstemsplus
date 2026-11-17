# scenario: Uninstall RB Stems Plus… (the app menu) with Stems Cache installed from another account:
# the short location check, the confirmation saying Stems Cache is another account's, "Stems Cache
# from another account" (Keep), then this account's files go and the app deletes itself, the last
# alert saying other accounts need the install command; once the app is gone its privacy
# permissions are reset (tccutil), the app's TCC rows gone. Destructive: the app is installed
# again before the next scenario (run.sh)
# commit: 817c5a7, 4e8711a, flow:uninstall
# tag: first
# compare: exact
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 0; fact_engine 0002; }
steps() {
  launch
  settle
  check '[ "$(tcc_rows)" -gt 0 ]' "the app has TCC rows before"
  menu "menu.App.Uninstall RB Stems Plus…"
  next_dialog 10000; settle              # Uninstall RB Stems Plus?
  answer Uninstall
  next_dialog 30000; settle              # Stems Cache from another account
  answer Keep
  next_dialog 30000; settle              # RB Stems Plus is uninstalled
  answer Done
  local i
  for i in $(seq 1 300); do [ -e "$APP" ] || break; sleep 0.1; done
  check '[ ! -e "$APP" ]' "the app deleted itself"
  check '[ ! -e "$SUP" ] && [ ! -e "$LOGS" ]' "this account's folders are gone"
  check '! cmp -s "$RBLIB" "$H/pristine/libonnxruntime.1.18.0.dylib"' "Stems Cache stays for the other account"
  for i in $(seq 1 100); do [ "$(tcc_rows)" = 0 ] && break; sleep 0.1; done
  check '[ "$(tcc_rows)" = 0 ]' "the app's privacy permissions were reset"
}
