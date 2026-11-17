# scenario: P6, P7, P8: a 5,000-track playlist chosen, its estimate streamed at full speed, the
# list scrolled, then the other 5,000-track playlist at 1 line/ms; PERF_ROUNDS times a launch
# (default 4), so each launch gives several events of each, every one counted (no first-event
# split). P10.after7 is perf-start's (once per launch).
# tag: Prepare
# mode: perf
# metrics: P6 P7 P8
# Playlist 50 streams at full speed, playlist 51 at 1 line/ms; both have the same 5,000 local tracks.
setup() {
  fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=200"; fact_engine 0002
  { playlist_line 50 "" "Big full speed" playlist 5000; playlist_line 51 "" "Big paced" playlist 5000; } | helper playlists
  for p in 50 51; do
    echo "@repeat 5000 {\"file\":\"/Users/$U/Music/Harness/big-{n}.mp3\"}" | helper "tracks-$p"
  done
  local est='{"file":"/Users/'"$U"'/Music/Harness/big-{n}.mp3","result":"prepare","seconds":240,"entries":22,"gb":0.06,"missing":0}'
  local end='{"estimate":5000,"ready":0,"prepare":5000,"skipped":0,"entries":110000,"runs":110000,"time_s":275000,"need_gb":300,"kept_gb":0,"share_gb":150,"max_gb":200,"cache_gb":1,"free_gb":30,"fits":false,"fit_files":2500,"max_gb_needed":400,"disk_ok":false,"asking":0,"folders":[]}'
  printf '@repeat 5000 %s\n%s\n' "$est" "$end" | helper estimate-50
  printf '@pace 1\n@repeat 5000 %s\n%s\n' "$est" "$end" | helper estimate-51
}
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  tab Prepare
  wait_for 10000 "prepare.outline.rows=2"
  CMD_WAIT_S=10 cmd quiet 800 8000
  local i r
  for i in $(seq 1 "${PERF_ROUNDS:-4}"); do
    mark "P6 full $i"
    select_playlist 50
    CMD_WAIT_S=70 cmd quiet 1500 60000
    mark "P8 scroll $i"
    for r in 250 1000 2000 3000 4000 4999 0; do cmd scroll "$r"; CMD_WAIT_S=5 cmd quiet 200 3000; done
    mark "P6 paced $i"
    select_playlist 51
    CMD_WAIT_S=70 cmd quiet 1500 60000
  done
  mark "end"
  flush
}
