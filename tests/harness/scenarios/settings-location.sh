# scenario: Settings with a folder on an external drive chosen (the USB stick): the location row
# names the drive, Use This Mac asks with only what the switch does (the drive's stems stay on
# it), then Settings shows This Mac. Change… isn't scripted: on macOS 26 the folder panel's content
# is drawn by another process, so mac-A's probe sees no buttons in it (" {}") and can neither pick
# a folder nor cancel it (the owner's round)
# commit: 31bceee, 5f6fc9a, bd8bfd2
# tag: first
# compare: exact
# usb: yes
setup() { fixture_prepare_ready; fixture_drive; }
steps() {
  launch
  settle
  menu "menu.App.Settings…"
  next_dialog 5000; settle               # Settings: "Stems Cache location: RBSPSTICK"
  answer "Use This Mac"
  next_dialog 5000; settle               # Use This Mac?
  answer "Use This Mac"
  next_dialog 5000; settle               # Settings: This Mac
  answer Cancel; wait_no_dialog 5000; settle
  check '! grep -q "^cache_dir" "$SUP/config.ini"' "config.ini names no drive now"
  quit_app
  usb detach                            # the next scenario meets no stick
}
