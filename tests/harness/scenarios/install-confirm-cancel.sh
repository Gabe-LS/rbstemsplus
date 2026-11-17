# scenario: each install asks first, naming what it does, from the window and from the menu;
# Cancel changes nothing. The menus as a modal dialog leaves them (disabled under it).
# commit: d043f9b, 0d74d89, 2bdabab, 38a4cbd
# tag: first
# compare: exact
setup() { fact_welcomed; }
steps() {
  launch
  settle
  click c.installPlusBtn
  next_dialog 5000; settle
  answer Cancel; wait_no_dialog 5000; settle
  menu "menu.Manage.Stems Cache.Install"
  next_dialog 5000; settle
  answer Cancel; wait_no_dialog 5000; settle
  check '[ ! -e "$SUP/busy" ]' "Cancel changed nothing (no busy marker)"
}
