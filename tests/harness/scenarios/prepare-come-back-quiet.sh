# scenario: coming back to the app re-checks the shown playlist quietly: what is shown stays (no
# "Checking…" where a state was known) until the new check says otherwise
# commit: 61a5393, 5764bd4, 178c9ed, e6d8c06, 236a1c0
# tag: Prepare
# compare: exact
# verbs: both
# property: no-checking-flash
# The second estimate (on coming back) waits at its first line until the scenario lets it go,
# so the frames while it runs are recorded: the rows must keep their results.
setup() { fixture_prepare_ready; fixture_library; estimate_friday; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  finder_front
  estimate_friday back                 # the re-check on coming back waits for each line
  activate
  settle
  check '[ "$(grep -c "estimate-2" "$FAKE/log/argv.log")" -ge 2 ]' "coming back ran the estimate again"
  for i in 1 2 3 4; do helper_go "back$i"; settle 250; done
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
}
