# scenario: Stems Cache paused for lack of space on the chosen drive (the USB stick filled up to
# 50 MB free, under its floor of a tenth of the disk): on coming back the light says it stopped
# saving stems, the Prepare tab checks again (the floor changed what it checks), Reinstall Stems
# Cache's confirmation says it will stop saving; space freed, coming back, all is well again
# commit: d7778ea, 664bfcd
# tag: first
# compare: exact
# usb: yes
setup() { fixture_prepare_ready; fixture_drive; fixture_library; estimate_friday; fill_stick 0; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  finder_front
  fill_stick 50
  VERB=reopen activate
  settle 1000
  tab Manage; settle
  click c.reinstallCacheBtn
  next_dialog 5000; settle               # Reinstall Stems Cache? (it will stop saving)
  answer Cancel; wait_no_dialog 5000; settle
  finder_front
  fill_stick 0
  VERB=reopen activate
  settle 1000
  quit_app
  usb detach                            # the next scenario meets no stick
}
