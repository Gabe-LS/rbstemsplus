# scenario: choosing a playlist lists its tracks at once, then the estimate fills the rows as its
# lines come; Prepare turns on only with the whole estimate; a folder and an intelligent playlist
# say why they can't be prepared
# commit: 38a4cbd, 236a1c0, 23585dd
# tag: Prepare
# compare: exact
# order: prepare.tracksTable.r2 < prepare.prepareBtn.en
# The fake helper prints one estimate line per step the scenario allows (@wait), so every
# intermediate frame is recorded in order.
setup() { fixture_prepare_ready; fixture_library; estimate_friday go; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  settle
  select_playlist 1; settle            # a folder
  select_playlist 4; settle            # an intelligent playlist
  select_playlist 2
  wait_for 5000 "prepare.tracksTable.rows=3"
  settle
  for i in 1 2 3; do helper_go "go$i"; settle 250; done
  wait_for 100 "prepare.prepareBtn.en=0" && echo "ok Prepare off before the estimate's last line" >> "$RUN/result"
  helper_go go4
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
}
