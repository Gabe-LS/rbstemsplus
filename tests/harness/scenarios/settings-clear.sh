# scenario: Settings' Clear Saved Stems (this Mac): with the Status tab in front it checks the
# prepared playlists again; with the Prepare tab in front it checks the playlist again and the
# hidden Status tab checks nothing; the sheet comes back without the stems
# commit: 664bfcd, 6c231bd
# tag: Prepare
# compare: properties
# allow: status.table.r0
# (the Status tab's row shows the set's name before or after its check's "Checking…" by timing:
# by its final value and as a subsequence of A's; every other field exactly)
setup() {
  fixture_prepare_ready; fixture_library; estimate_friday
  fact_kept 3 4 "$(track warmup-a)" "$(track warmup-b)"
  helper estimate-3 <<EOF
{"estimate":2,"ready":2,"prepare":0,"fits":true,"fit_files":2,$ESTIMATE_END}
EOF
  saved_stems
}
# stems saved on this Mac, as Stems Cache leaves them (one model folder, one entry)
saved_stems() {
  local d="$UH/Library/Caches/rbstemsplus/0cc50d877629fda906562d08236b32a3e9934e64c4ea8f3ab7fc43f40976e772/ab"
  mkdir -p "$d" && mkfile 3m "$d/ab12cd34" && own "$UH/Library/Caches/rbstemsplus"
}
clear_stems() {
  menu "menu.App.Settings…"
  next_dialog 5000; settle
  answer_like "Clear Saved Stems"
  next_dialog 5000; settle               # the confirmation
  answer Clear
  next_dialog 5000; settle               # Settings again
  answer Cancel; wait_no_dialog 5000; settle
}
steps() {
  launch
  settle
  tab Status
  wait_for 5000 "status.table.rows=1"
  settle
  local s; s="$(calls_of estimate-3)"
  clear_stems
  check '[ ! -e "$UH/Library/Caches/rbstemsplus" ]' "the saved stems were cleared"
  check '[ "$(calls_of estimate-3)" -gt "$s" ]' "the Status tab in front checked again"
  saved_stems
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  s="$(calls_of estimate-3)"
  local p; p="$(calls_of estimate-2)"
  clear_stems
  wait_for 10000 "prepare.prepareBtn.en=1"
  settle
  check '[ "$(calls_of estimate-2)" -gt "$p" ]' "the Prepare tab in front checked again"
  check '[ "$(calls_of estimate-3)" = "$s" ]' "the hidden Status tab checked nothing"
}
