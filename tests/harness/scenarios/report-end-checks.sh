# scenario: Help › Create Report from the Status tab: the Manage tab comes to the front with the
# spinner and "Creating a report…", the work queue's "Report saved" (the zip on the Desktop), Done;
# back on the Status tab the prepared playlists are checked again
# commit: b606a39, 0a63096
# tag: first
# compare: exact
# mask: report \d{4}-\d\d-\d\d \d{6}
# (the zip's name holds the clock time)
setup() {
  fixture_prepare_ready; fixture_library
  fact_kept 3 4 "$(track warmup-a)" "$(track warmup-b)"
  helper estimate-3 <<EOF
{"estimate":2,"ready":2,"prepare":0,"fits":true,"fit_files":2,$ESTIMATE_END}
EOF
}
steps() {
  launch
  settle
  tab Status
  wait_for 5000 "status.table.rows=1"
  settle
  local s; s="$(calls_of estimate-3)"
  menu "menu.Help.Create Report"
  next_dialog 120000; settle             # Report saved
  check 'ls "$UH/Desktop/RB Stems Plus report "*.zip >/dev/null 2>&1' "the report is on the Desktop"
  answer Done; wait_no_dialog 5000; settle
  tab Status; settle
  check '[ "$(calls_of estimate-3)" -gt "$s" ]' "the Status tab checked again"
}
