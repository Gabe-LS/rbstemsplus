# scenario: an action the last run left half-done (its busy marker, its process gone): the launch
# offers to continue it; Cancel drops the marker
# commit: 1c0ee4e, feb5819, 13fff35
# tag: first
# compare: exact
setup() { fact_welcomed; fact_plus_on; fact_chosen 1 0; fact_busy reinstall-plus "Reinstall Demucs v4"; }
steps() {
  launch
  next_dialog 10000; settle
  answer Cancel; wait_no_dialog 5000; settle
  check '[ ! -e "$SUP/busy" ]' "Cancel dropped the busy marker"
}
