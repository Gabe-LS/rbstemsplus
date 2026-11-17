# scenario: when a check can't run (Stems Cache turned off in config.ini meanwhile), coming back
# empties the track list instead of keeping the last tracks marked Checking…, and says why;
# turned on again, the next coming back checks
# commit: cfe858c, e6d8c06, 99ad35d
# tag: Prepare
# compare: exact
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
  fact_config "rekordbox_model=1" "max_gb=20" "enabled=0"
  VERB=reopen activate
  settle
  finder_front
  fact_config "rekordbox_model=1" "max_gb=20"
  VERB=reopen activate
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
}
