# scenario: rekordbox's folder gone from /Applications (moved away) with Stems Cache's root folder
# still there: Uninstall Stems Cache asks to put a moved rekordbox back before removing the root
# folder ("rekordbox missing", Cancel / Remove); Cancel keeps it; again, Remove removes it (the
# root script, no password: sudo needs none here)
# commit: e2beb43, cec793f
# tag: first
# compare: exact
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1"; fact_sudo_nopasswd; fact_rekordbox_moved; }
steps() {
  launch
  settle
  click c.uninstallCacheBtn
  next_dialog 5000; settle
  answer Uninstall                       # the confirmation
  next_dialog 30000; settle              # rekordbox missing
  answer Cancel
  wait_for 30000 "c.spinner.anim=0"
  settle
  check '[ -d "$ROOTDIR" ]' "Cancel kept the root folder"
  if QUIET_FAIL=1 click c.uninstallCacheBtn; then
    next_dialog 5000; settle
    answer Uninstall
    next_dialog 30000; settle
    answer Remove
    wait_for 60000 "c.spinner.anim=0"
    settle
    check '[ ! -e "$ROOTDIR" ]' "Remove removed the root folder"
  else
    echo "note: Uninstall Stems Cache is off after Cancel (recorded)" >> "$RUN/result"
  fi
  quit_app
  fact_rekordbox_back
}
