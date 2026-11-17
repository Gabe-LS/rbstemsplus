# scenario: the Status tab's prepared playlists: the library's track counts, "Checking…" while a
# set is checked (only where no state is known), each set checked against its playlist as it is
# now (--playlist), an emptied playlist "No tracks in the playlist" by the check's counts, a
# deleted playlist checked by the set's own files; Prepare Again opens the playlist's folder
# commit: b606a39, 6c7380f, d76b06f, dc50c5f, c5cb4bc, 1ab82da, a310fc2
# tag: Prepare
# compare: exact
# allow: prepare.tracksTable.r0 prepare.tracksTable.r1 prepare.tracksTable.r2
# Prepare Again shows the Prepare tab (its check as it comes to the front) and chooses the playlist
# (a second check): in A the two checks overlap, so whether the rows show "Checking…" once or twice
# depends on timing. Those three rows are compared by their final values and order of first showing.
setup() {
  fixture_prepare_ready; fixture_library
  fact_kept 2 9 "$(track friday-a)" "$(track friday-b)" "$(track friday-c)"
  fact_kept 3 4 "$(track warmup-a)" "$(track warmup-b)"
  fact_kept 9 6 "$(track gone-a)"
  helper estimate-2 <<EOF
@pace 60
@wait s2
{"file":"$(track friday-a)","result":"ready","entries":40,"gb":0.12}
{"file":"$(track friday-b)","result":"prepare","seconds":312,"entries":28,"gb":0.08,"missing":3}
{"file":"$(track friday-c)","result":"ready","entries":17,"gb":0.05}
{"estimate":3,"ready":2,"prepare":1,"fits":true,"fit_files":3,$ESTIMATE_END}
EOF
  helper estimate-3 <<EOF
@wait s3
{"estimate":0,"ready":0,"prepare":0,"fits":true,"fit_files":0,$ESTIMATE_END}
EOF
  helper estimate-keep-9 <<EOF
@pace 60
{"file":"$(track gone-a)","result":"ready","entries":40,"gb":0.12}
{"estimate":1,"ready":1,"prepare":0,"fits":true,"fit_files":1,$ESTIMATE_END}
EOF
  # playlist 9 was deleted: its --tracks and its --playlist estimate fail
  printf '@stderr playlist 9 not found\n@exit 1\n' | helper tracks-9
  printf '@stderr playlist 9 not found\n@exit 1\n' | helper estimate-9
}
steps() {
  launch
  settle
  tab Status
  wait_for 5000 "status.table.rows=3"
  settle
  helper_go s2; settle 250
  helper_go s3; settle 250
  check 'grep "estimate-2" "$FAKE/log/argv.log" | grep -q "	--playlist	2"' "a set is checked against its playlist as it is now"
  cmd select-set 0; settle
  cmd doubleclick-set 0                 # Prepare Again: the Prepare tab, the playlist chosen
  wait_for 5000 "tab.sel=1"
  settle
}
