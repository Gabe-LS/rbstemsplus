# scenario: Settings… (window button and Cmd+, menu): the sheet's fields, location row and
# buttons; the menus under it; Save with new limits: the Prepare tab checks again (a setting
# changed what it checked); a bad value is refused with its own alert
# commit: 745073f, 8712a51, bd8bfd2, 5f6fc9a, 31bceee, e6d8c06, 664bfcd
# tag: first
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
  menu "menu.App.Settings…"
  next_dialog 5000; settle
  cmd fill 0 abc
  answer Save
  next_dialog 5000; settle    # the refusal
  answer OK
  next_dialog 5000; settle    # Settings again
  cmd fill 0 25
  answer Save
  :
  settle
  check 'grep -q "max_gb=25" "$SUP/config.ini"' "Save wrote the limit"
  check '[ "$(grep -c "estimate-2" "$FAKE/log/argv.log")" -ge 2 ]' "the Prepare tab checked again"
}
