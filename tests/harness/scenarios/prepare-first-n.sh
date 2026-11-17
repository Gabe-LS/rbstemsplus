# scenario: a playlist that doesn't fit: the sheet offers Prepare the First N Tracks; the run gets
# the whole playlist with --first N (the set lets go of tracks that left it), and its end says
# "isn't fully prepared"
# commit: 9e52915, 692e695, 2a53f9a, 38a4cbd
# tag: Prepare
# compare: exact
# allow: prepare.prepareBtn.en prepare.info.str
# (a run's last lines reach the window either one by one (the pipe's handler) or together (the
# termination handler): A shows one intermediate frame of these two more or fewer at the run's
# end; they are compared by final values and order)
setup() {
  fixture_prepare_ready; fixture_library
  {
    echo "@pace 60"
    printf '{"file":"%s","result":"prepare","seconds":312,"entries":28,"gb":0.08,"missing":0}\n' "$(track friday-a)" "$(track friday-b)" "$(track friday-c)"
    printf '{"estimate":3,"ready":0,"prepare":3,"skipped":0,"entries":84,"runs":84,"time_s":210,"need_gb":24,"kept_gb":0,"share_gb":15,"max_gb":20,"cache_gb":1.2,"free_gb":2,"fits":false,"fit_files":2,"max_gb_needed":32.1,"disk_ok":true,"asking":0,"folders":[]}\n'
  } | helper estimate-2
  run_friday run
}
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  click prepare.prepareBtn
  next_dialog 5000
  settle
  cmd answer "Prepare the First 2 Tracks"
  wait_for 5000 "prepare.pauseBtn.hid=0"
  check 'grep "run-2" "$FAKE/log/argv.log" | grep -q "	--first	2"' "the run has the whole playlist and --first 2"
  for i in 1 2 3 4 5; do helper_go "run$i"; settle 200; done
  wait_for 8000 "prepare.prepareBtn.hid=0"
  settle
}
