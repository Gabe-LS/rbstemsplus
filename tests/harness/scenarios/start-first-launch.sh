# scenario: the first launch: nothing installed, the welcome sheet, then the Manage tab
# commit: b78c365 (welcome sheet), 13fff35 (start-up), start flow
# tag: first
# compare: exact
# Records what the window shows from the first frame: the welcome sheet over the Manage tab (the
# first tab when nothing is installed), both lights off, the install buttons on.
setup() { :; }
steps() {
  launch
  answer Continue
  :
  settle
  cmd fields              # the field table: every view and menu item's id
  check '[ "$(count_of dialog)" = 1 ]' "one dialog: the welcome"
}
