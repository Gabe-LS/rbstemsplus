# scenario: a chosen drive (the virtual USB stick) pulled out and plugged in again while the app is
# in front, with the Prepare tab showing a checked playlist: the light and the tabs say to connect
# it, then all is well again and the tab checks again. Then with rekordbox running and its bridge
# still saying the drive is fine (it hasn't noticed yet): pulled out, the light doesn't ask for a
# restart; the bridge noticing (not connected), plugged in again ("in a few seconds" while the
# bridge hasn't used it again), the bridge using it again
# commit: 99ad35d, c674d1f
# tag: Prepare
# compare: exact
# usb: yes
setup() { fixture_prepare_ready; fixture_drive; fixture_library; estimate_friday; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  local n; n="$(calls_of estimate-2)"
  usb detach; settle 1000
  usb attach; settle 1000
  wait_for 10000 "prepare.prepareBtn.en=1"
  settle
  check '[ "$(calls_of estimate-2)" -gt "$n" ]' "the tab checked again once the drive was back"
  local dir="$STICKCACHE"
  fact_rekordbox_open
  fact_bridge_state available "" "$dir"; settle 500
  usb detach; settle 1000                 # the bridge hasn't noticed
  fact_bridge_state unavailable not-connected "$dir"; settle 500
  usb attach; settle 1000                 # connected; the bridge's next check uses it
  fact_bridge_state available "" "$dir"; settle 500
  fact_rekordbox_closed
  quit_app
  usb detach                            # the next scenario meets no stick
}
