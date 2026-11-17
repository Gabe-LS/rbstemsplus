# scenario: Quit (the app menu's Quit, as Cmd+Q) while Create Report runs: nothing in rekordbox
# changes, so the quit doesn't wait; no report is left on the Desktop, and the next launch offers
# nothing (Create Report leaves no busy marker)
# commit: flow:quit-report
# tag: first
# compare: exact
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1"; fact_long_log; }
steps() {
  launch
  settle
  menu "menu.Help.Create Report"
  wait_for 10000 "c.spinner.anim=1"
  settle 200
  menu "menu.App.Quit RB Stems Plus"
  local i
  for i in $(seq 1 100); do pgrep -x "RB Stems Plus" >/dev/null || break; sleep 0.1; done
  check '! pgrep -x "RB Stems Plus" >/dev/null' "the app quit at once"
  check '! ls "$UH/Desktop/RB Stems Plus report "*.zip >/dev/null 2>&1' "no report on the Desktop"
  check '[ ! -e "$SUP/busy" ]' "no busy marker"
  launch
  settle
  check '[ "$(count_of dialog)" = 0 ]' "the next launch asks nothing"
}
