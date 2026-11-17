# scenario: P5: click to the first visible reaction (tabs, Check Again), 30 clicks a launch; each
# "mark click N FIELD" names the reaction P5 waits for (Check Again: the tracks table reloads;
# a tab: tabShown)
# tag: first
# mode: perf
# metrics: P5
setup() {
  fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1" "max_gb=20"; fact_engine 0002
  playlist_line 2 "" "Friday" playlist 3 | helper playlists
  for t in a b c; do printf '{"file":"%s"}\n' "$(track "friday-$t")"; done | helper tracks-2
  helper estimate-2 <<EOF
{"file":"$(track friday-a)","result":"ready","entries":40,"gb":0.12}
{"file":"$(track friday-b)","result":"prepare","seconds":312,"entries":28,"gb":0.08,"missing":0}
{"file":"$(track friday-c)","result":"prepare","seconds":185,"entries":17,"gb":0.05,"missing":0}
{"estimate":3,"ready":1,"prepare":2,"skipped":0,"entries":85,"runs":45,"time_s":112,"need_gb":0.13,"kept_gb":0.12,"share_gb":15,"max_gb":20,"cache_gb":1.2,"free_gb":30,"fits":true,"fit_files":3,"max_gb_needed":0.4,"disk_ok":true,"asking":0,"folders":[]}
EOF
}
steps() {
  launch
  CMD_WAIT_S=20 cmd quiet 1500 15000
  mark settled
  tab Prepare; CMD_WAIT_S=10 cmd quiet 800 8000
  select_playlist 2; CMD_WAIT_S=10 cmd quiet 800 8000
  local i
  for i in $(seq 1 "${PERF_ROUNDS:-10}"); do
    mark "click $i prepare.tracksTable"; click prepare.checkBtn; CMD_WAIT_S=10 cmd quiet 800 8000
    mark "click $i tabShown"; tab Manage; CMD_WAIT_S=10 cmd quiet 500 8000
    mark "click $i tabShown"; tab Prepare; CMD_WAIT_S=10 cmd quiet 800 8000
  done
  flush
}
