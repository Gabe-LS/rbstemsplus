# scenario: the start launch, 4 per side (thresholds.txt's @launches): P1, P2, P3 (location
# trigger), P10 idle 60 s after the start with P12.start and its wakeups (the CPU over that same
# window), then the 5,000-track playlist chosen once at full speed, the list scrolled, and P10
# after it (P10.after7). Every event counts (no first-event split); the gated ones here are P1,
# P2 and P10.
# tag: first
# mode: perf
# metrics: P1 P2 P3 P10.idle P10.after7 P12.start
# P10's idle form waits IDLE_S after the start (60 s in the full form; PERF_IDLE_S overrides it
# for a smoke run).
setup() {
  fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=200"; fact_engine 0002
  # perf-playlist-5000's two playlists (the same outline); only the full-speed one is chosen here
  { playlist_line 50 "" "Big full speed" playlist 5000; playlist_line 51 "" "Big paced" playlist 5000; } | helper playlists
  echo "@repeat 5000 {\"file\":\"/Users/$U/Music/Harness/big-{n}.mp3\"}" | helper tracks-50
  local est='{"file":"/Users/'"$U"'/Music/Harness/big-{n}.mp3","result":"prepare","seconds":240,"entries":22,"gb":0.06,"missing":0}'
  local end='{"estimate":5000,"ready":0,"prepare":5000,"skipped":0,"entries":110000,"runs":110000,"time_s":275000,"need_gb":300,"kept_gb":0,"share_gb":150,"max_gb":200,"cache_gb":1,"free_gb":30,"fits":false,"fit_files":2500,"max_gb_needed":400,"disk_ok":false,"asking":0,"folders":[]}'
  printf '@repeat 5000 %s\n%s\n' "$est" "$end" | helper estimate-50
}
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  mark "idle.start"
  top_idle "${PERF_IDLE_S:-60}" start
  mark "idle.start end"
  footprint_now idle
  tab Prepare
  wait_for 10000 "prepare.outline.rows=2"
  CMD_WAIT_S=10 cmd quiet 800 8000
  mark "P6 full"
  select_playlist 50
  CMD_WAIT_S=70 cmd quiet 1500 60000
  mark "P8 scroll"
  local r
  for r in 250 1000 2000 3000 4000 4999 0; do cmd scroll "$r"; CMD_WAIT_S=5 cmd quiet 200 3000; done
  footprint_now after7
  mark "end"
  flush
}
